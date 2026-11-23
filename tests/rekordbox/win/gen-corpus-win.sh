#!/bin/bash
# The corpus's Windows additions (corpus/win: T2's Windows rows and round 1's edges), beside the
# shared corpus/v1 (helper/tests/perf.sh corpus DIR): generated tones + beat + noise only (no
# recordings), each file its own frequencies, so no two chunks share a key. Needs ffmpeg (aac_at:
# macOS's AAC encoder). Usage: gen-corpus-win.sh DIR. Writes DIR/*.{wav,flac,mp4,aac,mp3} and
# DIR/SHA256SUMS. RBKIT_HE_FFMPEG can name an ffmpeg with libfdk_aac for the two HE profiles.
# pc-session.sh stage --corpus takes the folder holding v1/ and win/.
set -euo pipefail
d="$1"; mkdir -p "$d"
ff=(ffmpeg -nostdin -hide_banner -loglevel error -y)
bx=(-fflags +bitexact -flags:a +bitexact -map_metadata -1)
beat="0.35*sin(2*PI*50*t)*exp(-6*mod(t,0.5))"
sig() {   # f1 f2 gain -> a two-channel expression (gain 0.25 = perf.sh's level; everything scaled)
  echo "($3/0.25)*(0.25*sin(2*PI*$1*t)*(0.6+0.4*sin(2*PI*0.5*t))+$beat+0.12*(random(0)-0.5))|($3/0.25)*(0.25*sin(2*PI*$2*t)*(0.6+0.4*sin(2*PI*0.3*t))+$beat+0.12*(random(1)-0.5))"
}
src() {   # out rate secs f1 f2 amp [channels-expr]
  "${ff[@]}" -f lavfi -i "aevalsrc=exprs='${7:-$(sig "$4" "$5" "$6")}':s=$2:d=$3" -c:a pcm_f64le "${bx[@]}" "$1"
}
t="$(mktemp -d "$d/.src.XXXXXX")"
trap 'rm -rf "$t"' EXIT
he_encoder="${RBKIT_HE_FFMPEG:-}"
# float WAV: quiet samples (peak about -60 dBFS) and values beyond +-1 (peak about 1.6), float32 and float64
src "$t/q.wav" 44100 30 0 0 0 "$(sig 415 523 0.0003)"
src "$t/hot.wav" 44100 30 0 0 0 "$(sig 440 554 0.6)"
"${ff[@]}" -i "$t/q.wav" -c:a pcm_f32le "${bx[@]}" "$d/w01-wav-f32-quiet.wav"
"${ff[@]}" -i "$t/q.wav" -c:a pcm_f64le "${bx[@]}" "$d/w02-wav-f64-quiet.wav"
"${ff[@]}" -i "$t/hot.wav" -c:a pcm_f32le "${bx[@]}" "$d/w03-wav-f32-over1.wav"
"${ff[@]}" -i "$t/hot.wav" -c:a pcm_f64le "${bx[@]}" "$d/w04-wav-f64-over1.wav"
# FLAC at the rates the Mac refuses (Q7)
for r in 11025:w05:466:587 352800:w06:494:622 384000:w07:523:659; do
  IFS=: read -r rate n f1 f2 <<< "$r"
  src "$t/$n.wav" "$rate" 20 "$f1" "$f2" 0.25
  "${ff[@]}" -i "$t/$n.wav" -c:a flac -sample_fmt s32 -bits_per_raw_sample 24 "${bx[@]}" "$d/$n-flac-24-$rate.flac"
done
# 5.1 FLAC and WAV (Q7)
six="$(sig 554 698 0.2)|0.2*sin(2*PI*587*t)+$beat|0.15*sin(2*PI*622*t)|0.3*sin(2*PI*55*t)|0.15*sin(2*PI*659*t)*(0.5+0.5*sin(2*PI*0.2*t))|0.15*sin(2*PI*740*t)"
src "$t/six.wav" 48000 30 0 0 0 "$six"
"${ff[@]}" -i "$t/six.wav" -c:a flac -sample_fmt s16 -channel_layout 5.1 "${bx[@]}" "$d/w08-flac-5.1.flac"
"${ff[@]}" -i "$t/six.wav" -c:a pcm_s16le -channel_layout 5.1 "${bx[@]}" "$d/w09-wav-5.1.wav"
# an .mp4 with a video track before the audio (Q3), and one raw ADTS .aac (round 1's)
src "$t/v.wav" 44100 20 587 740 0.25
"${ff[@]}" -f lavfi -i "color=c=gray:s=160x120:r=10:d=20" -i "$t/v.wav" -map 0:v -map 1:a -c:v mpeg4 -q:v 10 -c:a aac_at -b:a 192k "${bx[@]}" -fflags +bitexact "$d/w10-mp4-video-first.mp4"
src "$t/r.wav" 44100 30 622 784 0.25
"${ff[@]}" -i "$t/r.wav" -c:a aac_at -b:a 192k "${bx[@]}" -f adts "$d/w11-aac-raw.aac"
# round 1's edges: shorter than a chunk (3 s), a silent stretch (10 s silence inside), mono, 22.05 kHz MP3
src "$t/s.wav" 44100 3 659 831 0.25
"${ff[@]}" -i "$t/s.wav" -c:a pcm_s16le "${bx[@]}" "$d/w12-wav-3s-short.wav"
src "$t/g.wav" 44100 40 0 0 0 "$(sig 698 880 0.25 | sed 's/^/if(between(t,15,25),0,1)*(/; s/|/)|if(between(t,15,25),0,1)*(/; s/$/)/')"
"${ff[@]}" -i "$t/g.wav" -c:a flac -sample_fmt s16 "${bx[@]}" "$d/w13-flac-silent-stretch.flac"
"${ff[@]}" -f lavfi -i "aevalsrc=exprs='0.3*sin(2*PI*740*t)*(0.6+0.4*sin(2*PI*0.4*t))+$beat+0.12*(random(0)-0.5)':s=48000:d=30" -c:a flac -sample_fmt s16 "${bx[@]}" "$d/w14-flac-mono-48k.flac"
src "$t/m.wav" 22050 30 784 988 0.25
"${ff[@]}" -i "$t/m.wav" -c:a libmp3lame -b:a 128k -write_xing 1 "${bx[@]}" "$d/w15-mp3-22k.mp3"
# the long-path case: this file's bytes are staged on the PC under a path over 260 characters
src "$t/l.wav" 44100 20 831 1047 0.25
"${ff[@]}" -i "$t/l.wav" -c:a flac -sample_fmt s16 "${bx[@]}" "$d/w16-flac-longpath.flac"
# Two enabled AAC tracks at the supported rate, shorter stereo first and longer mono second.
src "$t/multi-first.wav" 44100 8 863 1061 0.25
src "$t/multi-last.wav" 44100 23 0 0 0 "0.25*sin(2*PI*887*t)*(0.6+0.4*sin(2*PI*0.5*t))+$beat"
"${ff[@]}" -i "$t/multi-first.wav" -i "$t/multi-last.wav" -map 0:a -map 1:a -c:a aac -b:a 128k \
  -disposition:a:0 default -disposition:a:1 default -streamid 0:42 -streamid 1:17 -use_stream_ids_as_track_ids 1 \
  "${bx[@]}" "$d/w16-mp4-two-audio.mp4"
# Q3: this Mac's standard ffmpeg may lack libfdk_aac. An explicit supporting encoder
# produces both profiles from different tones so each has distinct samples.
if [ -n "$he_encoder" ]; then
  src "$t/he.wav" 44100 30 727 983 0.22
  src "$t/hev2.wav" 44100 30 749 997 0.22
  "$he_encoder" -nostdin -hide_banner -loglevel error -y -i "$t/he.wav" -c:a libfdk_aac -profile:a aac_he -b:a 48k "${bx[@]}" "$t/w17-aac-he.m4a"
  "$he_encoder" -nostdin -hide_banner -loglevel error -y -i "$t/hev2.wav" -c:a libfdk_aac -profile:a aac_he_v2 -b:a 32k "${bx[@]}" "$t/w18-aac-he-v2.m4a"
  mv "$t/w17-aac-he.m4a" "$t/w18-aac-he-v2.m4a" "$d/"
elif [ ! -f "$d/w17-aac-he.m4a" ] || [ ! -f "$d/w18-aac-he-v2.m4a" ]; then
  echo "HE-AAC and HE-AAC v2 need RBKIT_HE_FFMPEG (libfdk_aac)"
fi
# Chunk 1 (zero-based) starts at hop - pre and is silent for one full model input.
length=485100; hop=483052; pre=1024
silence="if(between(n,$((hop-pre)),$((hop-pre+length-1))),0,1)"
gated="$(sig 757 1009 0.21 | sed "s/^/$silence*(/; s/|/)|$silence*(/; s/\$/)/")"
src "$t/silent.wav" 44100 30 0 0 0 "$gated"
"${ff[@]}" -i "$t/silent.wav" -c:a flac -sample_fmt s16 "${bx[@]}" "$d/w19-flac-chunk-silent.flac"
# Chunk 0 has pre leading zeros. Its exclusive end in the file is length - pre.
for edge in '0:w20:769:1021' '1:w21:787:1031'; do
  IFS=: read -r extra name f1 f2 <<< "$edge"
  src "$t/$name.wav" 44100 12 "$f1" "$f2" 0.25
  "${ff[@]}" -i "$t/$name.wav" -af "atrim=end_sample=$((length-pre+extra))" -c:a pcm_s16le "${bx[@]}" "$d/$name-wav-chunk-edge.wav"
done
# 5.1 without the resampling path, on frequencies distinct from w08/w09.
six44="$(sig 811 1049 0.2)|0.2*sin(2*PI*823*t)+$beat|0.15*sin(2*PI*827*t)|0.3*sin(2*PI*61*t)|0.15*sin(2*PI*839*t)|0.15*sin(2*PI*853*t)"
src "$t/six44.wav" 44100 30 0 0 0 "$six44"
"${ff[@]}" -i "$t/six44.wav" -c:a pcm_s16le -channel_layout 5.1 "${bx[@]}" "$d/w22-wav-5.1-44100.wav"
(cd "$d" && shasum -a 256 -- *.* > SHA256SUMS)
echo "made $(grep -c . "$d/SHA256SUMS") files into $d"

#!/bin/sh
# X-Streemly — generate the test streams (CLAUDE.md §7 M0, §8).
#
#   tests/fixtures/make_fixtures.sh <out_dir>
#
# Every stream carries the A/V-sync pattern used by the harness
# (HARNESS_AVSYNC): a 2-frame white flash on black and a 100 ms 1 kHz beep at
# the start of every second. Needs ffmpeg (libx264, libx265) and openssl.
#
#   hls2/       live-style HLS: master.m3u8 with 720p + 1080p variants
#               (H.264 + AAC, 25 fps, 2 s segments, 20 s). tests/server.py
#               turns each variant's vod.m3u8 into a looping live.m3u8.
#   i1080.ts    1080i25 H.264 (tff) + AAC, 10 s
#   i576.ts     576i25 MPEG-2 (tff) + MP2, 10 s
#   p2997.ts    1080p29.97 H.264 + AC-3, 10 s
#   p50.ts      720p50 H.264 + AAC, 10 s
#   hevc.ts     720p25 HEVC + E-AC-3, 6 s
#   tls/        self-signed certificate for localhost (server.py --tls)
set -eu

OUT=${1:?usage: make_fixtures.sh <out_dir>}
mkdir -p "$OUT"
FF="ffmpeg -nostdin -hide_banner -loglevel error -y"

# video: black, white for 2 frames at the start of every second
video() { # size rate seconds
   echo "color=c=black:s=$1:r=$2:d=$3,drawbox=c=white:t=fill:enable='lt(mod(n\,round($2)),2)'"
}
# audio: 100 ms 1 kHz beep at the start of every second
audio() { # seconds
   echo "aevalsrc='if(lt(mod(t\,1)\,0.1)\,0.5*sin(2*PI*1000*t)\,0)|if(lt(mod(t\,1)\,0.1)\,0.5*sin(2*PI*1000*t)\,0)':s=48000:d=$1"
}

X264="-c:v libx264 -preset ultrafast -pix_fmt yuv420p"

echo "== hls2 (720p + 1080p variants)"
for v in 720:1280x720:1500k 1080:1920x1080:4000k; do
   name=${v%%:*}; rest=${v#*:}; size=${rest%%:*}; rate=${rest#*:}
   mkdir -p "$OUT/hls2/v$name"
   $FF -f lavfi -i "$(video "$size" 25 20)" -f lavfi -i "$(audio 20)" \
      $X264 -b:v "$rate" -g 50 -keyint_min 50 -sc_threshold 0 -c:a aac -b:a 128k \
      -f hls -hls_time 2 -hls_playlist_type vod \
      -hls_segment_filename "$OUT/hls2/v$name/seg_%03d.ts" "$OUT/hls2/v$name/vod.m3u8"
done
cat > "$OUT/hls2/master.m3u8" <<'EOF'
#EXTM3U
#EXT-X-VERSION:3
#EXT-X-STREAM-INF:BANDWIDTH=1700000,RESOLUTION=1280x720,FRAME-RATE=25.000,CODECS="avc1.42c01f,mp4a.40.2"
v720/live.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=4300000,RESOLUTION=1920x1080,FRAME-RATE=25.000,CODECS="avc1.42c028,mp4a.40.2"
v1080/live.m3u8
EOF
sed 's/live\.m3u8/vod.m3u8/' "$OUT/hls2/master.m3u8" > "$OUT/hls2/master_vod.m3u8"

echo "== i1080.ts (1080i25 H.264)"
$FF -f lavfi -i "$(video 1920x1080 50 10),tinterlace=interleave_top,setfield=tff" -f lavfi -i "$(audio 10)" \
   $X264 -flags +ildct+ilme -x264-params tff=1 -b:v 6M -g 25 -c:a aac -b:a 128k \
   -f mpegts "$OUT/i1080.ts"

echo "== i576.ts (576i25 MPEG-2)"
$FF -f lavfi -i "$(video 720x576 50 10),tinterlace=interleave_top,setfield=tff,setsar=64/45" -f lavfi -i "$(audio 10)" \
   -c:v mpeg2video -flags +ildct+ilme -top 1 -b:v 5M -g 12 -c:a mp2 -b:a 192k \
   -f mpegts "$OUT/i576.ts"

echo "== p2997.ts (1080p29.97 H.264 + AC-3)"
$FF -f lavfi -i "$(video 1920x1080 30000/1001 10)" -f lavfi -i "$(audio 10)" \
   $X264 -b:v 5M -g 30 -c:a ac3 -b:a 192k -f mpegts "$OUT/p2997.ts"

echo "== p50.ts (720p50 H.264)"
$FF -f lavfi -i "$(video 1280x720 50 10)" -f lavfi -i "$(audio 10)" \
   $X264 -b:v 3M -g 50 -c:a aac -b:a 128k -f mpegts "$OUT/p50.ts"

echo "== hevc.ts (720p25 HEVC + E-AC-3)"
$FF -f lavfi -i "$(video 1280x720 25 6)" -f lavfi -i "$(audio 6)" \
   -c:v libx265 -preset ultrafast -x265-params log-level=error -pix_fmt yuv420p -g 25 \
   -c:a eac3 -b:a 192k -f mpegts "$OUT/hevc.ts"

echo "== tls (self-signed, localhost)"
mkdir -p "$OUT/tls"
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj "/CN=localhost" \
   -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" \
   -keyout "$OUT/tls/key.pem" -out "$OUT/tls/cert.pem" 2>/dev/null

echo "fixtures in $OUT"

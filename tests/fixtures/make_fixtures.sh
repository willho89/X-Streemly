#!/bin/sh
# X-Streemly — generate the test streams (CLAUDE.md §7 M0, §8).
#
#   tests/fixtures/make_fixtures.sh <out_dir>
#
# Every stream carries the A/V-sync pattern used by the harness
# (HARNESS_AVSYNC): a 2-frame white flash on black and a 100 ms 1 kHz beep
# (-20 dBFS) at the start of every second. A moving test pattern (testsrc2)
# fills the top eighth of the picture, above the harness's 5x5 sampling grid,
# so colour, geometry and motion can be checked by eye. Needs ffmpeg
# (libx264, libx265) and openssl.
#
#   hls2/       live-style HLS: master.m3u8 with 720p + 1080p variants
#               (H.264 + AAC, 25 fps, 2 s segments, 20 s). tests/server.py
#               turns each variant's vod.m3u8 into a looping live.m3u8.
#   hls_i1080/  live-style HLS, 1080i25 H.264 (tff) + AC-3, 20 s (live.m3u8)
#   i1080.ts    1080i25 H.264 (tff) + AAC, 10 s
#   i576.ts     576i25 MPEG-2 (tff) + MP2, 10 s
#   p2997.ts    1080p29.97 H.264 + AC-3, 10 s
#   p50.ts      720p50 H.264 + AAC, 10 s
#   hevc.ts     720p25 HEVC + E-AC-3, 6 s
#   hls_change/ live HLS: 720p + stereo AC-3, then (discontinuity) 1080p + 5.1 AC-3
#   wrap.ts     timestamps that wrap at 2^33 (26.5 h) 2.5 s in
#   tracks.ts   three audio tracks with language tags (eng, spa, eng described video)
#   hls_tracks/ the same as live HLS
#   parser_*.m3u  channel lists that exercise the M3U parser
#   tls/        self-signed certificate for localhost (server.py --tls)
set -eu

OUT=${1:?usage: make_fixtures.sh <out_dir>}
mkdir -p "$OUT"
FF="ffmpeg -nostdin -hide_banner -loglevel error -y"

# video: black with a test-pattern strip on top, white for 2 frames at the
# start of every second
video() { # WxH rate seconds
   w=${1%x*}; h=${1#*x}; sh=$(( (h / 8) / 2 * 2 ))
   echo "color=c=black:s=$1:r=$2:d=$3[bg];testsrc2=s=${w}x$sh:r=$2:d=$3[ts];[bg][ts]overlay=0:0,drawbox=c=white:t=fill:enable='lt(mod(n\,round($2)),2)'"
}
# audio: 100 ms 1 kHz beep at the start of every second
audio() { # seconds
   echo "aevalsrc='if(lt(mod(t\,1)\,0.1)\,0.1*sin(2*PI*1000*t)\,0)|if(lt(mod(t\,1)\,0.1)\,0.1*sin(2*PI*1000*t)\,0)':s=48000:d=$1"
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

echo "== hls_i1080 (live-style HLS, 1080i25 H.264 + AC-3, 20 s)"
mkdir -p "$OUT/hls_i1080"
$FF -f lavfi -i "$(video 1920x1080 50 20),tinterlace=interleave_top,setfield=tff" -f lavfi -i "$(audio 20)" \
   $X264 -flags +ildct+ilme -x264-params tff=1 -b:v 6M -g 50 -keyint_min 50 -sc_threshold 0 -c:a ac3 -b:a 192k \
   -f hls -hls_time 2 -hls_playlist_type vod \
   -hls_segment_filename "$OUT/hls_i1080/seg_%03d.ts" "$OUT/hls_i1080/vod.m3u8"

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

echo "== hls_change (live HLS: 720p + stereo AC-3, discontinuity, 1080p + 5.1 AC-3)"
mkdir -p "$OUT/hls_change"
$FF -f lavfi -i "$(video 1280x720 25 10)" -f lavfi -i "$(audio 10)" \
   $X264 -b:v 2M -g 50 -keyint_min 50 -sc_threshold 0 -c:a ac3 -b:a 192k \
   -f hls -hls_time 2 -hls_playlist_type vod \
   -hls_segment_filename "$OUT/hls_change/a_%03d.ts" "$OUT/hls_change/a.m3u8"
$FF -f lavfi -i "$(video 1920x1080 25 10)" \
   -f lavfi -i "$(audio 10 | sed 's/|/|/')" -filter_complex "[1:a]pan=5.1|FL=c0|FR=c1|FC=c0|LFE=0*c0|BL=0.5*c0|BR=0.5*c1[a]" \
   -map 0:v -map "[a]" $X264 -b:v 4M -g 50 -keyint_min 50 -sc_threshold 0 -c:a ac3 -b:a 384k \
   -f hls -hls_time 2 -hls_playlist_type vod \
   -hls_segment_filename "$OUT/hls_change/b_%03d.ts" "$OUT/hls_change/b.m3u8"
{ sed -n '1,/^#EXT-X-MEDIA-SEQUENCE/p' "$OUT/hls_change/a.m3u8"
  grep -E '^(#EXTINF|a_)' "$OUT/hls_change/a.m3u8"
  echo "#EXT-X-DISCONTINUITY"
  grep -E '^(#EXTINF|b_)' "$OUT/hls_change/b.m3u8"
  echo "#EXT-X-ENDLIST"; } > "$OUT/hls_change/vod.m3u8"

echo "== wrap.ts (720p25 H.264 + AAC whose timestamps wrap at 2^33 after ~2.5 s)"
$FF -f lavfi -i "$(video 1280x720 25 12)" -f lavfi -i "$(audio 12)" \
   $X264 -b:v 2M -g 25 -c:a aac -b:a 128k -output_ts_offset 95441.2 -f mpegts "$OUT/wrap.ts"

echo "== tracks.ts (3 audio tracks: eng 1 kHz, spa 500 Hz, eng described video 2 kHz)"
tone() { # seconds hz
   echo "aevalsrc='if(lt(mod(t\,1)\,0.1)\,0.1*sin(2*PI*$2*t)\,0)|if(lt(mod(t\,1)\,0.1)\,0.1*sin(2*PI*$2*t)\,0)':s=48000:d=$1"
}
$FF -f lavfi -i "$(video 1280x720 25 10)" -f lavfi -i "$(tone 10 1000)" -f lavfi -i "$(tone 10 500)" -f lavfi -i "$(tone 10 2000)" \
   -map 0:v -map 1:a -map 2:a -map 3:a $X264 -b:v 2M -g 25 -c:a aac -b:a 128k \
   -metadata:s:a:0 language=eng -metadata:s:a:1 language=spa -metadata:s:a:2 language=eng \
   -disposition:a:0 default -disposition:a:2 visual_impaired -f mpegts "$OUT/tracks.ts"

echo "== hls_tracks (live HLS with the same three audio tracks)"
mkdir -p "$OUT/hls_tracks"
$FF -i "$OUT/tracks.ts" -map 0 -c copy -f hls -hls_time 2 -hls_playlist_type vod \
   -hls_segment_filename "$OUT/hls_tracks/seg_%03d.ts" "$OUT/hls_tracks/vod.m3u8"

echo "== playlists for the parser (BOM + CRLF + quoted commas; Latin-1; relative entries)"
printf '\357\273\277#EXTM3U x-tvg-url="http://example.com/epg.xml"\r\n' > "$OUT/parser_utf8.m3u"
printf '#EXTINF:-1 tvg-id="One.uk" tvg-logo="http://x/1.png" group-title="News, UK",One News (1080p) [Geo-blocked]\r\n' >> "$OUT/parser_utf8.m3u"
printf 'http://example.com/one.m3u8\r\n' >> "$OUT/parser_utf8.m3u"
printf '#EXTGRP:Music\r\n#EXTINF:-1 tvg-chno="7",Two, the channel (720p) [Not 24/7]\r\n' >> "$OUT/parser_utf8.m3u"
printf '#EXTVLCOPT:http-referrer=https://two.example/\r\n#EXTVLCOPT:http-user-agent=TwoAgent/1.0\r\n' >> "$OUT/parser_utf8.m3u"
printf 'https://example.com/two/index.m3u8\r\n' >> "$OUT/parser_utf8.m3u"
printf '#KODIPROP:inputstream.adaptive.license_type=com.widevine.alpha\r\n#EXTINF:-1 tvg-name="Three Name",\r\n' >> "$OUT/parser_utf8.m3u"
printf 'three/playlist.m3u8\r\n#EXTINF:-1,Caf\303\251 TV\r\n/abs/cafe.ts\r\n' >> "$OUT/parser_utf8.m3u"
printf '#EXTM3U\n#EXTINF:-1 group-title="T\351l\351",T\351l\351 Une\nhttp://example.com/tele.ts\n' > "$OUT/parser_latin1.m3u"

echo "== tls (self-signed, localhost)"
mkdir -p "$OUT/tls"
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj "/CN=localhost" \
   -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" \
   -keyout "$OUT/tls/key.pem" -out "$OUT/tls/cert.pem" 2>/dev/null

echo "fixtures in $OUT"

#!/bin/sh
# X-Streemly — make check (CLAUDE.md §7 M0, §8, D-013).
#
#   tests/check.sh <core.so> <harness> <fixture_dir> <build_dir>
#
# Starts tests/server.py (HTTPS and plain HTTP) on local ports, tunes the
# fixtures through .strm files and checks the core's log. No internet.
set -u

CORE=$1 HARNESS=$2 FIX=$3 BUILD=$4
WORK=$BUILD/check
mkdir -p "$WORK"
PIDS=""
LOG=""
NAME=""
MAX_RUN_US=5000    # nothing may block retro_run (D-001)
MAX_UNLOAD_MS=500

cleanup() {
   for p in $PIDS; do kill "$p" 2>/dev/null; done
   PIDS=""
}
trap cleanup EXIT INT TERM

fail() {
   echo "   $NAME: FAILED: $*"
   if [ -n "$LOG" ] && [ -f "$LOG" ]; then tail -40 "$LOG" | sed 's/^/     | /'; fi
   exit 1
}

# start_server <var> [--tls]: sets <var> to the base URL.
start_server() {
   var=$1; shift
   out=$WORK/server_$var.log
   if [ "${1:-}" = "--tls" ]; then
      python3 -I tests/server.py "$FIX" --tls "$FIX/tls/cert.pem" "$FIX/tls/key.pem" > "$out" 2>&1 &
      scheme=https
   else
      python3 -I tests/server.py "$FIX" > "$out" 2>&1 &
      scheme=http
   fi
   PIDS="$PIDS $!"
   i=0
   while ! grep -q '^PORT ' "$out" 2>/dev/null; do
      i=$((i + 1)); [ $i -gt 50 ] && { NAME=server; LOG=$out; fail "didn't start"; }
      sleep 0.1
   done
   eval "$var=$scheme://localhost:$(awk '/^PORT /{print $2; exit}' "$out")"
}

# run <name> <url|content> <frames> [env...]: harness log in $LOG.
run() {
   NAME=$1 url=$2 frames=$3; shift 3
   LOG=$WORK/$NAME.log
   case $url in
      -) content=- ;;
      *) content=$WORK/$NAME.strm; echo "$url" > "$content" ;;
   esac
   env "$@" "$HARNESS" "$CORE" "$content" "$frames" "$WORK" > "$LOG" 2>&1 || fail "harness exited with $?"
}

expect()    { grep -qE -- "$1" "$LOG" || fail "expected /$1/"; }
expect_no() { ! grep -qE -- "$1" "$LOG" || fail "unexpected /$1/"; }

# Timing and audio-count invariants every run must meet.
check_common() {
   max=$(awk '/^run_us:/{print $7}' "$LOG")
   [ -n "$max" ] && [ "$max" -le $MAX_RUN_US ] || fail "retro_run max ${max:-?} us > $MAX_RUN_US"
   unload=$(awk '/^unload_ms/{print int($2)}' "$LOG")
   [ -n "$unload" ] && [ "$unload" -le $MAX_UNLOAD_MS ] || fail "unload ${unload:-?} ms > $MAX_UNLOAD_MS"
   grep -qE 'audio_frames ([0-9]+) \(expected \1\)' "$LOG" || fail "audio sample count != frames x rate/fps"
}

ok() { echo "   $NAME: ok${1:+ ($1)}"; }

# check_avsync <min matched flashes> <max |offset| ms>: lip sync on the
# flash + beep pattern every fixture carries, and no clicks.
check_avsync() {
   line=$(grep '^avsync: flashes' "$LOG") || fail "no avsync report"
   matched=$(echo "$line" | sed -E 's/.*matched ([0-9]+).*/\1/')
   worst=$(echo "$line" | sed -E 's/.*min ([-+0-9.]+) max ([-+0-9.]+).*/\1 \2/' |
           awk '{a = $1 < 0 ? -$1 : $1 + 0; b = $2 < 0 ? -$2 : $2 + 0; print (a > b ? a : b)}')
   [ "$matched" -ge "$1" ] || fail "only $matched flashes matched a beep (want >= $1)"
   awk -v w="$worst" -v m="$2" 'BEGIN { exit !(w <= m) }' || fail "A/V offset $worst ms > $2 ms"
   grep -qE '^avsync: clicks 0 ' "$LOG" || fail "clicks: $(grep '^avsync: clicks' "$LOG")"
   AVS="A/V within ${worst} ms over $matched flashes"
}

# segments of an HLS variant requested from a server log
segs() { grep -c "REQ GET .*/$2/seg_" "$WORK/server_$1.log"; }

start_server HTTPS --tls
start_server HTTP

echo "== M1: play over HTTPS (mbedTLS) and HTTP at native resolution, in sync"

run hls2_https "$HTTPS/hls2/master.m3u8" 900 HARNESS_AVSYNC=1
expect 'opened in [0-9]+ ms: format hls, live, 2 program\(s\), 4 stream\(s\)'
expect 'variant: program 1, 1920x1080, 4300 kb/s'
expect 'playing after'
expect 'geometry 1920x1080 aspect 1\.7778'
expect_no 'open failed|Signal lost|audio underrun'
check_common
check_avsync 12 40
v720=$(segs HTTPS v720)
[ "$v720" -le 3 ] || fail "the 720p variant kept downloading ($v720 segments)"
ok "1080p variant (720p: $v720 probe segments), $AVS, $(awk '/^run_us:/{print "retro_run max "$7" us"}' "$LOG"), $(awk '/^cpu:/{print $2" CPU"}' "$LOG")"

run i1080 "$HTTPS/i1080.ts" 720 HARNESS_AVSYNC=1
expect 'video: h264 1920x1080 yuv420p, interlaced tff'
expect 'deinterlacing with bwdif at field rate'
expect 'end of stream'
check_common
check_avsync 9 40
shown=$(grep -oE 'shown [0-9]+' "$LOG" | tail -1 | cut -d' ' -f2)
[ "$shown" -ge 480 ] || fail "only $shown pictures shown: not field rate"
ok "1080i25 -> $shown pictures in 10 s, $AVS"

run i576_http "$HTTP/i576.ts" 720 HARNESS_AVSYNC=1
expect 'video: mpeg2video 720x576 yuv420p, interlaced tff'
expect 'geometry 720x576 aspect 1\.7778'
check_common
check_avsync 9 40
ok "576i MPEG-2, 16:9 from 64:45 pixels, plain HTTP, $AVS"

run p2997 "$HTTPS/p2997.ts" 720 HARNESS_AVSYNC=1
expect 'audio: ac3 48000 Hz'
expect 'geometry 1920x1080 '
check_common
check_avsync 9 40
ok "1080p29.97 + AC-3, $AVS"

run p50 "$HTTPS/p50.ts" 720 HARNESS_AVSYNC=1
expect 'geometry 1280x720 '
check_common
check_avsync 9 40
ok "720p50, $AVS"

run hevc "$HTTPS/hevc.ts" 480 HARNESS_AVSYNC=1
expect 'video: hevc 1280x720'
expect 'audio: eac3 48000 Hz'
check_common
check_avsync 5 40
ok "HEVC + E-AC-3, $AVS"

run local_file "$(pwd)/$FIX/p50.ts" 120
expect 'format mpegts, fixed length'
expect 'playing after'
check_common
ok "local file in a .strm"

echo "== M1: options"

run cap720 "$HTTPS/hls2/master.m3u8" 300 HARNESS_OPT_xstreemly_max_resolution=720
expect 'variant: program 0, 1280x720, 1700 kb/s .* within the resolution cap'
expect 'geometry 1280x720 '
check_common
ok "Max Resolution 720p -> 720p variant"

run cap720_scale "$HTTPS/i1080.ts" 240 HARNESS_OPT_xstreemly_max_resolution=720
expect 'geometry 1280x720 '
check_common
ok "Max Resolution 720p -> 1080i scaled to 1280x720"

run deint_off "$HTTPS/i1080.ts" 300 HARNESS_OPT_xstreemly_deinterlace=off HARNESS_AVSYNC=1
expect_no 'Cannot convert|convert:'
expect_no 'deinterlacing'
check_common
check_avsync 3 40
ok "Deinterlace off: woven frames"

run yadif "$HTTPS/i1080.ts" 720 HARNESS_OPT_xstreemly_deinterlace=yadif HARNESS_AVSYNC=1
expect 'deinterlacing with yadif at field rate'
check_common
check_avsync 9 40
shown=$(grep -oE 'shown [0-9]+' "$LOG" | tail -1 | cut -d' ' -f2)
[ "$shown" -ge 480 ] || fail "only $shown pictures shown: not field rate"
ok "Deinterlace yadif: $shown pictures in 10 s, $AVS"

echo "== M1: stalls and endings"

run stall "$HTTPS/_hiccup/12/9000/hls2/master.m3u8" 1500 HARNESS_AVSYNC=1
expect 'audio underrun .*re-buffering'
expect 'resumed after'
check_common
check_avsync 15 40
ok "9 s stall: holds the picture, re-buffers, resumes; $AVS"

run gaps "$HTTPS/_gap/4/hls2/master.m3u8" 1500 HARNESS_AVSYNC=1
expect 'playing after'
check_common
check_avsync 15 40
joins=$(grep -c 'discontinuity of' "$LOG")
[ "$joins" -le 2 ] || fail "$joins joins: forward gaps must be kept, only the 20 s loop joined"
ok "missing segments kept as gaps ($joins loop joins), $AVS"

run vod_end "$HTTPS/p50.ts" 780
expect 'end of stream'
expect 'geometry 640x360 '
check_common
ok "file plays to its end, then the card"

echo "== failures never block the frontend"

run http_404 "$HTTPS/_status/404/hls2/master.m3u8" 60
expect 'open failed: No signal \(.*404'
expect 'OSD: No signal'
check_common
ok "404 -> No signal"

run unload_connecting "$HTTPS/_delay/10000/hls2/master.m3u8" 30
expect_no 'opened in'
check_common
ok "unload while connecting: $(awk '/^unload_ms/{print $2}' "$LOG") ms"

run no_content - 30
expect 'OSD: No content'
check_common
ok "no content"

echo "== all checks passed"

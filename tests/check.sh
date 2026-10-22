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

start_server HTTPS --tls
start_server HTTP

echo "== M0: open over HTTPS (mbedTLS) and HTTP, log what each source offers"

run hls2_https "$HTTPS/hls2/master.m3u8" 240
expect 'opened in [0-9]+ ms: format hls, live, 2 program\(s\), 4 stream\(s\)'
expect 'program 0: bandwidth 1700000'
expect 'program 1: bandwidth 4300000'
expect 'video: h264 1280x720 '
expect 'video: h264 1920x1080 '
expect 'audio: aac 48000 Hz stereo'
expect 'first packet [0-9]+ ms after tune'
expect 'OSD: h264 '
expect_no 'open failed|Signal lost'
check_common
ok "$(awk '/opened in/{print $6" ms to open"; exit}' "$LOG"), $(awk '/^run_us:/{print "retro_run max "$7" us"}' "$LOG")"

run i1080 "$HTTPS/i1080.ts" 120
expect 'video: h264 1920x1080 SAR 1:1 25\.000 fps scan i tff'
expect 'audio: aac 48000 Hz'
expect 'end of stream'
check_common
ok "1080i tff"

run i576_http "$HTTP/i576.ts" 120
expect 'video: mpeg2video 720x576 SAR 64:45 25\.000 fps scan i tff'
expect 'audio: mp2 48000 Hz'
check_common
ok "576i tff, plain HTTP"

run p2997 "$HTTPS/p2997.ts" 120
expect 'video: h264 1920x1080 SAR 1:1 29\.970 fps scan p'
expect 'audio: ac3 48000 Hz'
check_common
ok "29.97p + AC-3"

run p50 "$HTTPS/p50.ts" 120
expect 'video: h264 1280x720 SAR 1:1 50\.000 fps scan p'
check_common
ok "720p50"

run hevc "$HTTPS/hevc.ts" 120
expect 'video: hevc 1280x720 '
expect 'audio: eac3 48000 Hz'
check_common
ok "HEVC + E-AC-3"

run local_file "$(pwd)/$FIX/p50.ts" 60
expect 'format mpegts, fixed length'
check_common
ok "local file in a .strm"

echo "== M0: failures never block the frontend"

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

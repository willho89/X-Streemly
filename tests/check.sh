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
# Nothing may block retro_run (D-001). Its own work must stay small (p99),
# and no tick may wait: a network wait or a held lock costs tens of ms or
# more. Single ticks of a few ms happen when the OS runs something else on
# a busy desktop (wall time without CPU time), so the worst tick gets room.
P99_RUN_US=5000
MAX_RUN_US=50000
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

# run <name> <url|content> <frames> [env...]: harness log in $LOG. A URL
# goes through a .strm; a path to an .m3u is loaded as it is.
run() {
   NAME=$1 url=$2 frames=$3; shift 3
   LOG=$WORK/$NAME.log
   case $url in
      -) content=- ;;
      http://*|https://*) content=$WORK/$NAME.strm; echo "$url" > "$content" ;;
      *.m3u|*.m3u8) content=$url ;;
      *) content=$WORK/$NAME.strm; echo "$url" > "$content" ;;
   esac
   env "$@" "$HARNESS" "$CORE" "$content" "$frames" "$WORK" > "$LOG" 2>&1 || fail "harness exited with $?"
}

expect()    { grep -qE -- "$1" "$LOG" || fail "expected /$1/"; }
expect_no() { ! grep -qE -- "$1" "$LOG" || fail "unexpected /$1/"; }

# Timing and audio-count invariants every run must meet.
check_common() {
   p99=$(awk '/^run_us:/{print $5}' "$LOG")
   max=$(awk '/^run_us:/{print $7}' "$LOG")
   [ -n "$p99" ] && [ "$p99" -le $P99_RUN_US ] || fail "retro_run p99 ${p99:-?} us > $P99_RUN_US"
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

echo "== M2: playlists"

run parser_utf8 "$FIX/parser_utf8.m3u" 2 HARNESS_DEBUG=1 HARNESS_FAST=1
expect 'lineup: 4 channel'
expect 'channel 1: "One News" group "News, UK" tvg-id "One.uk" 1080p geo-blocked url http://example.com/one.m3u8'
expect 'channel 2: "Two, the channel" group "Music" tvg-id "" 720p not-24/7 url https://example.com/two/index.m3u8 ua TwoAgent/1.0 referrer https://two.example/'
expect 'channel 3: "Three Name" group "Music" .* DRM url .*fixtures/three/playlist.m3u8'
expect 'channel 4: "Café TV" .* url /abs/cafe.ts'
ok "BOM, CRLF, quoted commas, #EXTGRP, #EXTVLCOPT, #KODIPROP, suffixes, relative URLs"

run parser_latin1 "$FIX/parser_latin1.m3u" 2 HARNESS_DEBUG=1 HARNESS_FAST=1
expect 'channel 1: "Télé Une" group "Télé"'
ok "Latin-1 list read as UTF-8"

# The 10-channel lineup over the test servers (tests/fixtures/channels.m3u.in).
LINEUP=$WORK/lineup.m3u
sed -e "s|@HTTPS@|$HTTPS|" -e "s|@HTTP@|$HTTP|" tests/fixtures/channels.m3u.in > "$LINEUP"
fresh_lineup() { rm -rf "$WORK/X-Streemly"; }

echo "== M2: channel changing"

fresh_lineup
run zap "$LINEUP" 1500 HARNESS_INPUT="300:UP,600:UP,900:UP,1150:B,1300:K7,1400:K1,1410:K0"
expect 'channel 2: Fixture News 720'
expect 'channel 3: Interlaced Sport'
expect 'channel 4: Dead Channel'
expect 'open failed: No signal \(Server returned 403'
expect 'channel 10: Unreachable'
expect_no 'channel 7:'
[ "$(grep -c 'channel 3: Interlaced Sport' "$LOG")" -eq 2 ] || fail "B didn't go back to channel 3"
slowest=$(grep -oE 'picture [0-9.]+ s after tune' "$LOG" | awk '{print $2}' | sort -n | tail -1)
[ "$(grep -c 'picture .* after tune' "$LOG")" -ge 4 ] || fail "too few channels reached a picture"
awk -v t="$slowest" 'BEGIN { exit !(t <= 1.0) }' || fail "a live channel took $slowest s to a picture"
check_common
ok "up/up/up (dead: 403 card), B = last, 7 refused (DRM), 1-0 = channel 10; slowest picture $slowest s"

fresh_lineup
taps=""; f=200; i=0
while [ $i -lt 20 ]; do taps="$taps${taps:+,}$f:UP"; f=$((f + 4)); i=$((i + 1)); done
run rapid_taps "$LINEUP" 500 HARNESS_INPUT="$taps"
[ "$(grep -c '\] channel [0-9]*: ' "$LOG")" -eq 2 ] || fail "20 quick taps must tune once, not $(($(grep -c '\] channel [0-9]*: ' "$LOG") - 1)) times"
expect 'channel 3: Interlaced Sport'
check_common
ok "20 quick taps: one tune, to channel 3 (20 steps over 9 playable channels)"

fresh_lineup
presses=""; f=200; i=0
while [ $i -lt 20 ]; do presses="$presses${presses:+,}$f:UP"; f=$((f + 40)); i=$((i + 1)); done
run rapid_zap "$LINEUP" 1100 HARNESS_INPUT="$presses"
[ "$(grep -c '\] channel [0-9]*: ' "$LOG")" -eq 21 ] || fail "expected 21 tunes"
check_common
ok "20 zaps 0.67 s apart: 21 tunes, $(awk '/^run_us:/{print "retro_run max "$7" us"}' "$LOG"), unload $(awk '/^unload_ms/{print $2}' "$LOG") ms"

fresh_lineup
run hold "$LINEUP" 400 HARNESS_INPUT="200:UP*90"
[ "$(grep -c '\] channel [0-9]*: ' "$LOG")" -eq 2 ] || fail "holding Up must tune once"
expect 'channel (8|9|10|1|2): '
check_common
ok "holding Up auto-repeats, tunes once on release"

fresh_lineup
run groups "$LINEUP" 500 HARNESS_INPUT="150:R,300:L"
expect 'channel 3: Interlaced Sport'
[ "$(grep -c 'channel 1: Fixture News' "$LOG")" -eq 2 ] || fail "L didn't go back to the News group"
check_common
ok "R = next group (Sport), L = previous group (News)"

fresh_lineup
run list "$LINEUP" 400 HARNESS_INPUT="150:SELECT,200:DOWN,220:DOWN,260:A"
expect 'channel 3: Interlaced Sport'
check_common
ok "channel list: Select, Down, Down, A"

fresh_lineup
run remember1 "$LINEUP" 300 HARNESS_INPUT="100:K5"
expect 'channel 5: SD Movies'
run remember2 "$LINEUP" 60 HARNESS_INPUT="20:B"
expect 'lineup: back on channel 5'
expect 'channel 5: SD Movies'
expect 'channel 1: Fixture News'
ok "the last channel (5) and the one before (1) survive a restart"

echo "== M3: iptv-org (a fake one on the test server) and lists at a URL"

IPTV=$FIX/iptv-fake
mkdir -p "$IPTV/countries" "$IPTV/languages"
sed -e "s|@HTTPS@|$HTTPS|" -e "s|@HTTP@|$HTTP|" tests/fixtures/iptv_uk.m3u.in > "$IPTV/countries/uk.m3u"
sed -e "s|@HTTPS@|$HTTPS|" -e "s|@HTTP@|$HTTP|" tests/fixtures/iptv_eng.m3u.in > "$IPTV/languages/eng.m3u"
# Lineup pinned to iptv-org: Auto would look for (and find) real HDHomeRuns on the LAN.
FAKE="XSTREEMLY_IPTV_BASE=$HTTPS/iptv-fake/ XSTREEMLY_CA_FILE=$FIX/tls/cert.pem HARNESS_OPT_xstreemly_source=iptv"
fresh_lineup

run iptv_start - 900 $FAKE
expect 'iptv-org: downloaded countries/uk.m3u'
expect 'iptv-org: 5 channels \(hidden: 1 geo-blocked, 0 not 24/7, 0 other filters, 1 duplicates\)'
expect 'channel 1 is dead'
expect 'channel 2: Fixture News'
expect 'picture .* after tune'
check_common
ok "no content: 5 channels (geo-blocked and a duplicate hidden), dead channel 1 skipped on its own"

run iptv_cache - 120 $FAKE
expect 'countries/uk.m3u from the cache'
expect_no 'channel 1: Dead News'
ok "cached list; starts past the channel known to be dead"

run iptv_offline - 600 XSTREEMLY_IPTV_BASE=https://127.0.0.1:1/ HARNESS_OPT_xstreemly_refresh=0 HARNESS_OPT_xstreemly_source=iptv
expect 'using the cached list'
expect 'picture .* after tune'
ok "offline: the cached list, however old"

run iptv_down - 400 $FAKE HARNESS_INPUT="150:DOWN"
expect 'channel 5: Fixture Music'
expect_no 'channel 1: Dead News'
ok "Down from channel 2 wraps to 5, skipping dead channel 1"

# Starts on channel 5 (where the last run left it): X marks it, Up goes on
# (skipping dead 1) to 2, Y comes back to the favourite.
run iptv_favs - 700 $FAKE HARNESS_INPUT="150:X,200:UP,500:Y"
expect 'lineup: back on channel 5'
expect 'channel 2: Fixture News'
[ "$(grep -c 'channel 5: Fixture Music' "$LOG")" -eq 2 ] || fail "Y didn't go back to the favourite"
[ -s "$WORK/X-Streemly/favourites.txt" ] || fail "favourites not saved"
ok "X marks a favourite (saved), Y goes back to it"

fresh_lineup
run iptv_verify - 120 XSTREEMLY_IPTV_BASE=$HTTPS/iptv-fake/ HARNESS_OPT_xstreemly_source=iptv
expect 'lineup: countries/uk.m3u: '
expect 'OSD: Channel list: '
ok "TLS is checked: the self-signed test server is refused without its certificate"

fresh_lineup
run iptv_category - 300 $FAKE HARNESS_OPT_xstreemly_category=Sports
expect 'iptv-org: 1 channels'
expect 'channel 1: Fixture Sport'
ok "Category Sports: only Outdoor;Sports matches"

fresh_lineup
run iptv_language - 300 $FAKE HARNESS_OPT_xstreemly_language=eng
expect 'iptv-org: 2 channels'
expect 'channel 1: Fixture News'
ok "Language English: the country's channels that are in the language list"

fresh_lineup
run iptv_not247 - 120 $FAKE HARNESS_OPT_xstreemly_hide_not_247=on
expect 'iptv-org: 4 channels \(hidden: 1 geo-blocked, 1 not 24/7'
ok "Hide part-time channels"

fresh_lineup
run remote_list "$HTTPS/iptv-fake/countries/uk.m3u" 300
expect 'channel list .*: 7 channel'
expect 'lineup: 7 channel'
ok "a .strm naming a channel list loads it (unfiltered, as the user's own)"

echo "== M3b: HDHomeRun (a fake one on the test server)"

HDHR=$FIX/hdhr-fake
mkdir -p "$HDHR"
sed -e "s|@HTTPS@|$HTTPS|" -e "s|@HTTP@|$HTTP|" tests/fixtures/hdhr_discover.json.in > "$HDHR/discover.json"
sed -e "s|@HTTPS@|$HTTPS|" -e "s|@HTTP@|$HTTP|" tests/fixtures/hdhr_lineup.json.in > "$HDHR/lineup.json"
TUNER="XSTREEMLY_HDHR=$HTTP/hdhr-fake HARNESS_OPT_xstreemly_source=hdhomerun"
fresh_lineup

run hdhr "-" 900 $TUNER HARNESS_DEBUG=1 HARNESS_INPUT="300:K3,304:KDOT,308:K2,500:UP"
expect 'hdhomerun: HDHomeRun FAKE at .*hdhr-fake, 2 tuners'
expect 'hdhomerun: 5 channels from HDHomeRun FAKE \(2 can.t be played'
expect 'channel 8.1: "FAKE DRM" group "Local TV" .* DRM'
expect 'channel 8.2: "FAKE AC4" .* no-codec'
expect 'channel 3.1: FAKE-HD'
expect 'channel 3.2: FAKE SD'
expect 'channel 49.10: BUSY'
expect_no 'channel 8.[12]: FAKE'
expect_no 'is dead'
expect 'picture .* after tune'
check_common
ok "discover.json + lineup.json: 3.1 / 3.2 / 49.10 numbers, typed 3.2, Up skips DRM and AC-4, a busy tuner isn't 'dead'"

fresh_lineup
run hdhr_both "-" 300 XSTREEMLY_HDHR=$HTTP/hdhr-fake HARNESS_OPT_xstreemly_source=both XSTREEMLY_IPTV_BASE=$HTTPS/iptv-fake/ XSTREEMLY_CA_FILE=$FIX/tls/cert.pem HARNESS_DEBUG=1
expect 'channel 3.1: "FAKE-HD"'
expect 'channel 1001: "Dead News"'
expect 'lineup: 10 channel'
ok "HDHomeRun + iptv-org: local channels first, iptv-org from 1001"

fresh_lineup
run hdhr_auto_none "-" 300 XSTREEMLY_HDHR=http://127.0.0.1:1 HARNESS_OPT_xstreemly_source=hdhomerun
expect 'OSD: Channel list: '
check_common
ok "HDHomeRun chosen but unreachable: NO CHANNELS card"

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

fresh_lineup
run no_content - 120 XSTREEMLY_IPTV_BASE=https://127.0.0.1:1/ HARNESS_OPT_xstreemly_source=iptv
expect 'lineup: countries/uk.m3u: '
expect 'OSD: Channel list: '
check_common
ok "no content and iptv-org unreachable (no cache): NO CHANNELS card, nothing blocks"

echo "== all checks passed"
